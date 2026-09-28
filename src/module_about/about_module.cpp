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
// 与设置页同一套视觉语言，非自绘 rect）。行高 24px、行内左 label 110px 区 +
// 右侧值右对齐，标题行 28px（icon 18px + 标题 18px/600）。
void aboutCard(eui::Ui& ui, const std::string& id, const core::theme::Tokens& t,
               float x, float y, float w, float h, unsigned int iconCode,
               const char* title, const AboutRow* rows, int rowCount) {
    constexpr float padX = 28.0f;
    ui.stack(id)
        .position(x, y)
        .size(w, h)
        .content([&] {
            ui_kit::cardFrame(ui, id, w, h, t);
            ui.text(id + ".icon")
                .position(padX, 18.0f)
                .size(20.0f, 20.0f)
                .icon(iconCode)
                .fontSize(18.0f)
                .color(t.colors.textSecondary)
                .build();
            ui.text(id + ".title")
                .position(padX + 24.0f, 16.0f)
                .size(w - padX * 2.0f - 24.0f, 22.0f)
                .text(title)
                .fontSize(t.sizes.fontSizeLarge)
                .lineHeight(22.0f)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            const float rowTop = 16.0f + 28.0f;
            for (int i = 0; i < rowCount; ++i) {
                const float rowY = rowTop + static_cast<float>(i) * 24.0f;
                ui.text(id + ".label." + std::to_string(i))
                    .position(padX, rowY)
                    .size(110.0f, 24.0f)
                    .text(rows[i].label)
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(24.0f)
                    .color(t.colors.textTertiary)
                    .build();
                ui.text(id + ".value." + std::to_string(i))
                    .position(padX, rowY)
                    .size(w - padX * 2.0f, 24.0f)
                    .text(rows[i].value)
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(24.0f)
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

// ---- 漂浮圆环动态背景（D-01 视觉增强；Spring 物理取自 EUI-NEO
// examples/magic_wand.cpp，onFrame/onMove 运行时契约同款）----
// awake 模式：鼠标移动唤醒漂移，弹簧全 settled + 超时后 compose 不再给
// 背景层挂 onFrame → 运行时零开销休眠（magic_wand 同款省电设计）。
// 状态机本体（Spring / FloatingRing / AboutAnimState / initRingLayer）在
// about_anim.h —— 独立成头是为了单测能直接驱动（可见度区间 / 漂移 /
// 休眠均可断言，2026-09-28「看不出动态效果」回归不再无人守）。

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

        // ========== 背景层：透明浮环动态背景（Spring + awake 模式）==========
        // 页面自身不铺底色（主窗 clearColor 透出，磨砂档主体 alpha 恒 1 才可见，
        // 见 home_module 同款注释）；本层仅作透明承载体，z 序最底、无 onClick，
        // 不拦截英雄/卡片。浮环在上述透明层之上、内容之下，作为背景装饰。
        AboutAnimState& anim = ui.state<AboutAnimState>("about.anim");
        if (!anim.initialized) {
            initRingLayer(anim, screen.width, screen.height);
        }
        auto bg = ui.rect("about.bg");
        bg.position(0.0f, 0.0f)
            .size(screen.width, screen.height)
            .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .cursor(eui::CursorShape::Arrow)
            .onMove([&anim](const eui::PointerEvent&, const eui::Rect&) {
                anim.wakeHold = 0.0f;  // 鼠标移动唤醒漂移
            });
        if (anim.awake()) {
            // 全 settled + 超时 → 下一帧 compose 不再挂此回调 → 运行时休眠
            bg.onFrame(
                [&anim, w = screen.width, h = screen.height](float dt) {
                    anim.step(dt, w, h);
                });
        }
        bg.build();
        // 浮环绘制（弹簧跟随位）+ 主题色现算（primary ↔ primarySoftBg mix）
        for (int i = 0; i < AboutAnimState::kRingCount; ++i) {
            const FloatingRing& ring = anim.rings[i];
            // 混色系数 = phase × 0.5（半程限幅）：浅色档 primarySoftBg
            // #F5E6DE 与 surface #F5F2EC 几乎同色，全幅混色会让 phase→1 的
            // 环即使透明度上调仍不可见（2026-09-28「看不出动态效果」根因
            // 之一）——限幅后每个环都落在 primary → 半程色之间，任何主题
            // 下都有可辨暖色
            const eui::Color ringColor = mixThemeColor(
                t.colors.primary, t.colors.primarySoftBg, ring.phase * 0.5f);
            const float px = anim.springX[i].position;
            const float py = anim.springY[i].position;
            ui.rect("about.ring." + std::to_string(i))
                .position(px - ring.radius, py - ring.radius)
                .size(ring.radius * 2.0f, ring.radius * 2.0f)
                .radius(ring.radius)
                .border(1.0f, ringColor)
                .opacity(ring.opacity)
                .build();
        }

        // 居中 1040px 主内容区（spec §布局概览；窄窗不滚动，D-04 目标 =
        // 默认窗口内完整可见）。2026-09-28 用户真机反馈「内容太窄」→ 720→1040，
        // 两张卡随之改上下堆叠、各占满 1040（并排时代的 516 宽已废）。页面宽 =
        // 窗口宽 − 侧边栏（展开 208，见 app_sidebar.cpp pageContentWidth）；
        // 最小窗宽 1330 → 页面宽 1122，居中后左右各余 41px，不溢出（页面宽只
        // 依赖最小窗**宽**；最小窗高由 core 层按工作区推导，
        // core::window_layout，本机 805）
        constexpr float kAboutContentW = 1040.0f;
        const float x = std::max(0.0f, (screen.width - kAboutContentW) / 2.0f);
        const float cardGap = 8.0f;

        // ========== hero 段（居中；图标 + 标题两件）==========
        // 顶部留白 = 两档间距 token 叠加（XXLarge 40 + XLarge 24 = 64）：草图
        // 观感区间 60-70px，单档 XXLarge(40) 偏小、双档 XXLarge(80) 越界，
        // 故用叠加推导值，不引裸魔法数。2026-09-28 删 hero 副标题三行后复核：
        // 该留白约束的是「页面顶边 → hero 起点」（hero 起笔仍是 56px 大图标），
        // 与 hero 内部有几行正文无关——维持 64 不变
        const float topPad = t.sizes.spacingXXLarge + t.sizes.spacingXLarge;
        float y = topPad;
        // hero 图标 48→56（= XXLarge 40 + Large 16，同属 token 叠加）：用户要
        // 「hero 更有存在感」，但字号受规范约束（页面标题 32 / 正文 15，见
        // CLAUDE.md「视觉规范」）不可动——空间感由加宽 + 顶部留白承担；图标是
        // 装饰尺寸、不在字号规范内，故适度上调一档（56 = 1.75× 页面标题）。
        // 不再往上（64 = 2×）的理由：图标盒高 = 字号，em 盒内上下余量更小，
        // 而本机 screencapture 被 TCC 拦、无截图手段验证字形不裁切
        const float heroIcon = t.sizes.spacingXXLarge + t.sizes.spacingLarge;
        ui.text("about.hero.icon")
            .position(x + (kAboutContentW - heroIcon) / 2.0f, y)
            .size(heroIcon, heroIcon)
            .icon(0xF5FC)  // fa-laptop-code（侧边栏 logo 同款）
            .fontSize(heroIcon)
            .color(t.colors.primary)
            .build();
        y += heroIcon + 8.0f;
        ui.text("about.hero.title")
            .position(x, y)
            .size(kAboutContentW, 40.0f)
            .text("DevDesk")
            .fontSize(t.sizes.fontSizePage)
            .lineHeight(40.0f)
            .fontWeight(700)
            .color(t.colors.textPrimary)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .build();
        // 原 hero 副标题三行（about.hero.sub / .build / .commit：版本行 /
        // 构建时刻 / commit+编译器）与下方「应用信息」卡逐项重复（用户真机
        // 反馈「卡片里已经有版本号之类的东西」）→ 2026-09-28 删除；标题到
        // 卡片用一档 XLarge(24) 间距（原三行文本流共 78px 的替代，标题与
        // 卡片的分组关系由间距本身表达）
        y += 40.0f + t.sizes.spacingXLarge;

        // ========== 卡 1 + 卡 2：上下堆叠（2026-09-28 用户真机反馈；原左右并排）==========
        // 每张卡占满内容区宽 1040（原并排宽 (1040−8)/2 = 516）；两卡各 6 行、
        // 等高 204（16 + 28 + 6×24 + 16），堆叠总高 204×2 + 卡距 8 = 416。高度
        // 预算（本页无滚动）：内容底 = 64+56+8+40+24+416 = 608；与页脚不重叠
        // 要求页面高 H ≥ 608 + 8 + 20 + 40 = 676 —— 最小窗页面高 805（core 层
        // 按工作区推导，core::window_layout）满足：页脚顶 805−40−20 = 745，
        // 卡片距页脚余 137px；工作区更大只会更宽松
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
        constexpr float infoH = 16.0f + 28.0f + 6.0f * 24.0f + 16.0f;
        aboutCard(ui, "about.card.app", t, x, y, cardW, infoH,
                  0xF05A /* fa-circle-info */, "应用信息", infoRows, 6);

        // 卡 2：依赖与框架（下，y = y + infoH + cardGap）
        const AboutRow depsRows[] = {
            {"GUI 框架", "EUI-NEO (OpenGL)", false},
            {"EUI-NEO", aboutEuiNeoLine(), true},
            {"数据库", "SQLite 3 + FTS5", false},
            {"HTTP", "libcurl (静态链接)", false},
            {"TLS", "OpenSSL 3.x (静态链接)", false},
            {"字体", "FreeType + FontAwesome 7", false},
        };
        constexpr float depsH = 16.0f + 28.0f + 6.0f * 24.0f + 16.0f;
        aboutCard(ui, "about.card.deps", t, x, y + infoH + cardGap, cardW, depsH,
                  0xF5FD /* fa-layer-group */, "依赖与框架", depsRows, 6);

        // ========== 页脚（贴页面底部）==========
        // 2026-09-28 用户真机反馈：footer 不再跟卡底流式排（原来紧跟卡底，页面
        // 下方一大片空），改贴页面底部——卡片与页脚之间的空白即呼吸区，随窗口
        // 高度自然伸缩。底边留白 = 页面 padding token（spacingXXLarge 40，与
        // 设置页 pad 同口径）。max() 护栏：本页无滚动，极矮窗下 footer 不回退
        // 到与卡片重叠
        constexpr float footerH = 20.0f;
        const float cardsBottom = y + infoH + cardGap + depsH;
        const float footerY = std::max(
            cardsBottom + cardGap,
            screen.height - t.sizes.spacingXXLarge - footerH);
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