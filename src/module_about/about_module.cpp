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
void openDirectoryButton(eui::Ui& ui, const std::string& id,
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

} // namespace

class AboutModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::About; }
    std::string displayName() const override { return "关于"; }
    std::string shortName() const override { return "关于"; }
    std::string iconGlyph() const override { return eui::utf8(0xF05A); }  // fa-circle-info

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();

        // 居中 720px 主内容区（spec §布局概览；窄窗不滚动，D-04 目标 =
        // 默认窗口 1600×1000 完整可见，总高约 840px）
        constexpr float kAboutContentW = 720.0f;
        constexpr float padX = 28.0f;  // 卡内左右边距（kDialogInsetX 语系）
        const float x = std::max(0.0f, (screen.width - kAboutContentW) / 2.0f);
        const float cardGap = 8.0f;

        // ========== hero 段（居中，spacingLarge 间距）==========
        float y = 24.0f;
        const float heroIcon = 48.0f;
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

        // ========== 卡 1：应用信息 ==========
        const AboutRow infoRows[] = {
            {"版本", aboutVersionLine(), false},
            {"Git commit", DEVDESK_GIT_REV, true},
            {"构建时间", aboutBuildLine(), false},
            {"编译器", DEVDESK_COMPILER, false},
            {"平台", aboutPlatformLine(), false},
            {"C++ 标准", aboutCppLine(), false},
        };
        constexpr float infoH = 16.0f + 28.0f + 6.0f * 24.0f + 16.0f;
        aboutCard(ui, "about.card.app", t, x, y, kAboutContentW, infoH,
                  0xF05A /* fa-circle-info */, "应用信息", infoRows, 6);
        y += infoH + cardGap;

        // ========== 卡 2：依赖与框架 ==========
        const AboutRow depsRows[] = {
            {"GUI 框架", "EUI-NEO (OpenGL)", false},
            {"EUI-NEO", aboutEuiNeoLine(), true},
            {"数据库", "SQLite 3 + FTS5", false},
            {"HTTP", "libcurl (静态链接)", false},
            {"TLS", "OpenSSL 3.x (静态链接)", false},
            {"字体", "FreeType + FontAwesome 7", false},
        };
        constexpr float depsH = 16.0f + 28.0f + 6.0f * 24.0f + 16.0f;
        aboutCard(ui, "about.card.deps", t, x, y, kAboutContentW, depsH,
                  0xF5FD /* fa-layer-group */, "依赖与框架", depsRows, 6);
        y += depsH + cardGap;

        // ========== 卡 3：数据（每行值 + 「打开」按钮）==========
        const float dataCardH = 16.0f + 28.0f + 3.0f * 40.0f + 16.0f;
        const std::string dataRows[][2] = {
            {"配置文件", ::core::paths::configFile().string()},
            {"日志文件", ::core::paths::logFile().string()},
            {"数据目录", ::core::paths::petsDir().string()},
        };
        {
            const std::string& dataId = "about.card.data";
            ui.stack(dataId)
                .position(x, y)
                .size(kAboutContentW, dataCardH)
                .content([&] {
                    ui_kit::cardFrame(ui, dataId, kAboutContentW, dataCardH, t);
                    ui.text(dataId + ".icon")
                        .position(padX, 18.0f)
                        .size(20.0f, 20.0f)
                        .icon(0xF07C)  // fa-folder-open
                        .fontSize(18.0f)
                        .color(t.colors.textSecondary)
                        .build();
                    ui.text(dataId + ".title")
                        .position(padX + 24.0f, 16.0f)
                        .size(kAboutContentW - padX * 2.0f - 24.0f, 22.0f)
                        .text("数据")
                        .fontSize(t.sizes.fontSizeLarge)
                        .lineHeight(22.0f)
                        .fontWeight(600)
                        .color(t.colors.textPrimary)
                        .build();
                    const float rowTop = 16.0f + 28.0f;
                    const float valueX = padX + 94.0f;
                    const float btnX = kAboutContentW - padX - 72.0f;
                    const float valueW = btnX - 8.0f - valueX;  // 值与按钮不重叠
                    for (int i = 0; i < 3; ++i) {
                        const float rowY = rowTop + static_cast<float>(i) * 40.0f;
                        ui.text(dataId + ".label." + std::to_string(i))
                            .position(padX, rowY + 8.0f)
                            .size(90.0f, 24.0f)
                            .text(dataRows[i][0])
                            .fontSize(t.sizes.fontSizeSmall)
                            .lineHeight(24.0f)
                            .color(t.colors.textTertiary)
                            .build();
                        ui.text(dataId + ".value." + std::to_string(i))
                            .position(valueX, rowY + 8.0f)
                            .size(valueW, 24.0f)
                            .text(dataRows[i][1])
                            .fontSize(t.sizes.fontSizeSmall)
                            .lineHeight(24.0f)
                            .color(t.colors.textSecondary)
                            .build();
                        openDirectoryButton(ui, dataId + ".open." + std::to_string(i),
                                            t, btnX, rowY + 5.0f, dataRows[i][1]);
                    }
                })
                .build();
        }
        y += dataCardH + cardGap;

        // ========== 页脚 ==========
        ui.text("about.footer")
            .position(x, y)
            .size(kAboutContentW, 20.0f)
            .text("© 2026 DevDesk")
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(20.0f)
            .color(t.colors.textTertiary)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .build();
    }
};

} // namespace module::about

extern "C" ::core::Module* create_about_module() {
    return new module::about::AboutModule();
}