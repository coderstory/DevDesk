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
// 组合行见 compose 的 about.hero.commit（"commit <rev> · <编译器>"）。

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

// 半隐式欧拉弹簧（framer-motion useSpring 同款）：软跟随漂移锚点，
// 静止后 settled() 为 true → awake() 注销帧回调 -> 休眠。
struct Spring {
    float position = 0, velocity = 0, target = 0, stiffness = 80, damping = 14, mass = 1;
    void setTarget(float t) { target = t; }
    void update(float dt) {
        // 半隐式欧拉
        float force = stiffness * (target - position);
        velocity = (velocity + force / mass * dt) / (1.0f + damping / mass * dt);
        position += velocity * dt;
    }
    bool settled() const { return std::abs(target - position) < 0.5f && std::abs(velocity) < 1.0f; }
};

// 单个浮环：静态参数 + 漂移锚点（x/y）；绘制位置 = 弹簧跟随位
//（锚点直线漂移 + 边界弹回，弹簧软跟 → trailing 有机感）。
struct FloatingRing {
    float x, y;            // 漂移锚点（逻辑圆点；边界弹回作用于此）
    float radius = 0.0f;   // 12-34px
    float opacity = 0.0f;  // 0.06-0.15（元素级透明度）
    float speedX = 0.0f, speedY = 0.0f;  // px/s，正负 15-30
    float phase = 0.0f;    // 0-1：primary ↔ primarySoftBg 色 mix 系数
    eui::Color color;      // 每个 compose 按当前 theme 现算（可切主题）
};

struct AboutAnimState {
    static constexpr int kRingCount = 7;            // 需求区间 6-8
    static constexpr float kWakeHoldSeconds = 2.0f; // 鼠标静止多久后休眠
    std::array<FloatingRing, kRingCount> rings;
    std::array<Spring, kRingCount> springX;         // x 轴弹簧（追锚点）
    std::array<Spring, kRingCount> springY;
    float wakeHold = 0.0f;   // 距上次鼠标移动的秒数
    bool initialized = false;

    // 全弹簧 settled + 超时 → 休眠（compose 不再挂 onFrame）
    bool awake() const {
        if (wakeHold < kWakeHoldSeconds) return true;
        for (int i = 0; i < kRingCount; ++i) {
            if (!springX[i].settled() || !springY[i].settled()) return true;
        }
        return false;
    }

    // 每帧：唤醒期内锚点直线漂移 + 碰撞边界弹回；弹簧软跟随。
    // 休眠前最后一帧仍推进弹簧，让位置收敛到锚点再撤回调。
    void step(float dt, float w, float h) {
        wakeHold += dt;
        const bool drift = wakeHold < kWakeHoldSeconds;
        for (int i = 0; i < kRingCount; ++i) {
            FloatingRing& ring = rings[i];
            Spring& sx = springX[i];
            Spring& sy = springY[i];
            if (drift) {
                ring.x += ring.speedX * dt;
                ring.y += ring.speedY * dt;
                const float m = ring.radius + 6.0f;  // 边界 margin（含弹簧 lag 余量）
                const float loX = m;
                const float hiX = std::max(m, w - m);
                if (ring.x <= loX) { ring.x = loX; ring.speedX = std::abs(ring.speedX); }
                else if (ring.x >= hiX) { ring.x = hiX; ring.speedX = -std::abs(ring.speedX); }
                const float loY = m;
                const float hiY = std::max(m, h - m);
                if (ring.y <= loY) { ring.y = loY; ring.speedY = std::abs(ring.speedY); }
                else if (ring.y >= hiY) { ring.y = hiY; ring.speedY = -std::abs(ring.speedY); }
                sx.setTarget(ring.x);
                sy.setTarget(ring.y);
            }
            sx.update(dt);
            sy.update(dt);
        }
    }
};

// 极小确定性 RNG（golden-ratio LCG）：初始散布每次启动可复现，
// 免 <random> 依赖（零头文件成本）。
struct RingRng {
    std::uint64_t s;
    explicit RingRng(std::uint64_t seed) : s(seed) {}
    float next() {  // [0,1)
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<float>((s >> 33) & 0xFFFFFFu) / 16777216.0f;
    }
};

// 首次进入页面：随机散布浮环（位置/半径/速度/透明度/色 mix 系数）。
// 弹簧初始 position = target → 直接 settled，首帧即可静默。
void initRingLayer(AboutAnimState& state, float w, float h) {
    RingRng rng(0x9E3779B97F4A7C15ULL);
    for (int i = 0; i < AboutAnimState::kRingCount; ++i) {
        FloatingRing& ring = state.rings[i];
        ring.radius = 12.0f + rng.next() * 22.0f;   // 12-34px
        const float m = ring.radius + 6.0f;
        ring.x = m + rng.next() * std::max(1.0f, w - 2.0f * m);
        ring.y = m + rng.next() * std::max(1.0f, h - 2.0f * m);
        const float sp = 15.0f + rng.next() * 15.0f;  // 15-30 px/s
        ring.speedX = rng.next() < 0.5f ? -sp : sp;
        ring.speedY = rng.next() < 0.5f ? -sp : sp;
        ring.opacity = 0.06f + rng.next() * 0.09f;    // 0.06-0.15
        ring.phase = rng.next();
        state.springX[i].setTarget(ring.x);
        state.springX[i].position = ring.x;
        state.springY[i].setTarget(ring.y);
        state.springY[i].position = ring.y;
    }
    state.wakeHold = 0.0f;
    state.initialized = true;
}

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
            FloatingRing& ring = anim.rings[i];
            ring.color = mixThemeColor(t.colors.primary,
                                       t.colors.primarySoftBg, ring.phase);
            const float px = anim.springX[i].position;
            const float py = anim.springY[i].position;
            ui.rect("about.ring." + std::to_string(i))
                .position(px - ring.radius, py - ring.radius)
                .size(ring.radius * 2.0f, ring.radius * 2.0f)
                .radius(ring.radius)
                .border(1.0f, ring.color)
                .opacity(ring.opacity)
                .build();
        }

        // 居中 1040px 主内容区（spec §布局概览；窄窗不滚动，D-04 目标 =
        // 默认窗口内完整可见）。2026-09-28 用户真机反馈「内容太窄」→ 720→1040：
        // 卡片宽随之 356→516。页面宽 = 窗口宽 − 侧边栏（展开 208，见
        // app_sidebar.cpp pageContentWidth），最小窗 1330×845 → 页面宽 1122，
        // 居中后左右各余 41px，不溢出
        constexpr float kAboutContentW = 1040.0f;
        const float x = std::max(0.0f, (screen.width - kAboutContentW) / 2.0f);
        const float cardGap = 8.0f;

        // ========== hero 段（居中，spacingLarge 间距）==========
        // 顶部留白 = 两档间距 token 叠加（XXLarge 40 + XLarge 24 = 64）：草图
        // 观感区间 60-70px，单档 XXLarge(40) 偏小、双档 XXLarge(80) 越界，
        // 故用叠加推导值，不引裸魔法数
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
        y += 40.0f + 6.0f;
        ui.text("about.hero.sub")
            .position(x, y)
            .size(kAboutContentW, 20.0f)
            .text(aboutVersionLine())
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(20.0f)
            .color(t.colors.textSecondary)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .build();
        y += 20.0f + 2.0f;
        ui.text("about.hero.build")
            .position(x, y)
            .size(kAboutContentW, 18.0f)
            .text(aboutBuildLine())
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(18.0f)
            .color(t.colors.textTertiary)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .build();
        y += 18.0f + 2.0f;
        ui.text("about.hero.commit")
            .position(x, y)
            .size(kAboutContentW, 18.0f)
            .text("commit " DEVDESK_GIT_REV " · " DEVDESK_COMPILER)
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(18.0f)
            .color(t.colors.textTertiary)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .build();
        y += 18.0f + 12.0f;

        // ========== 卡 1 + 卡 2：左右并排（宽 (720-8)/2 = 356，等高取 max）==========
        const float cardW = (kAboutContentW - cardGap) / 2.0f;

        // 卡 1：应用信息（左）
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

        // 卡 2：依赖与框架（右，x = x + cardW + cardGap）
        const AboutRow depsRows[] = {
            {"GUI 框架", "EUI-NEO (OpenGL)", false},
            {"EUI-NEO", aboutEuiNeoLine(), true},
            {"数据库", "SQLite 3 + FTS5", false},
            {"HTTP", "libcurl (静态链接)", false},
            {"TLS", "OpenSSL 3.x (静态链接)", false},
            {"字体", "FreeType + FontAwesome 7", false},
        };
        constexpr float depsH = 16.0f + 28.0f + 6.0f * 24.0f + 16.0f;
        aboutCard(ui, "about.card.deps", t, x + cardW + cardGap, y, cardW, depsH,
                  0xF5FD /* fa-layer-group */, "依赖与框架", depsRows, 6);

        // ========== 页脚（贴页面底部）==========
        // 2026-09-28 用户真机反馈：footer 不再跟卡底流式排（原来紧跟卡底，页面
        // 下方一大片空），改贴页面底部——卡片与页脚之间的空白即呼吸区，随窗口
        // 高度自然伸缩。底边留白 = 页面 padding token（spacingXXLarge 40，与
        // 设置页 pad 同口径）。max() 护栏：本页无滚动，极矮窗下 footer 不回退
        // 到与卡片重叠
        constexpr float footerH = 20.0f;
        const float cardsBottom = y + std::max(infoH, depsH);
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