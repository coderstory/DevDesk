// 关于页独立模块（S3.8 UAT6.2 整改）。
// 实底白卡 + 标题左对齐 + 描述列表（标签/值层级）+ Ghost 复制图标 + 信息分组。
// 玻璃体系已退役（S6.1）。
// 平台/架构由 CMake 配置期注入宏，本文件只消费。
#include "core/component_theme.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "core/window/window_backend.h"  // core::window::setClipboardText（复制）

#include <ui_kit/light_toast.h>  // showLightToast（复制成功 toast）

#include <eui/dsl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

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
// UAT6 构建行扩展（根 CMakeLists 注入；手工编译兜底同上）
#ifndef DEVDESK_BUILD_HOST
#define DEVDESK_BUILD_HOST "unknown"
#endif
#ifndef DEVDESK_BUILD_SYSTEM
#define DEVDESK_BUILD_SYSTEM "unknown"
#endif

namespace module::about {

namespace {
// 纯数字解析（前导空格忽略——__DATE__ 日域 <10 为 " 3" 形；非数字/空 → false）。
// 不用 std::stoi：异常路径与本仓库「不抛异常」约定相悖，且这里只需十进制全数字。
bool parseAllDigits(std::string s, int& out) {
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    if (s.empty()) return false;
    int v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}
}  // namespace

// ---- 构建行纯函数（UAT6）：外部链接供 devdesk_tests 经 extern 声明单测；
// 不建头文件（本批冻结清单只开 about_module.cpp / 根 CMakeLists / 对应测试）----

// __DATE__/__TIME__ → 标准格式「年-月-日 时:分:秒」。两编译器（MSVC/GCC）的
// __DATE__ 均为 "Mmm dd yyyy"（日 <10 时日域双空格填充，如 "Oct  3 2026"）、
// __TIME__ 均为 "hh:mm:ss"。解析失败不造数据：原样返回 `date + " " + time`。
std::string formatBuildStamp(const std::string& date, const std::string& time);

// CMake 原料（根 CMakeLists 注入的 "CMAKE_SYSTEM_NAME CMAKE_SYSTEM_VERSION"，
// 如 "Windows 10.0.26200" / "Darwin 24.5.0"）→ 用户口径系统标签：
// Windows build 号（版本第三段）≥22000 → "Windows 11"、<22000 → "Windows 10"
//（缺版本号则只留系统名）；Darwin → "macOS"（附原版本号）；其余系统原样。
std::string normalizeBuildSystemLabel(const std::string& raw);

std::string formatBuildStamp(const std::string& date, const std::string& time) {
    // 格式前提：11 字符 "Mmm dd yyyy"（日域双空格填充）+ 8 字符 "hh:mm:ss"。
    // 解析失败不造数据：原样返回 `date + " " + time`。
    if (date.size() != 11 || time.size() != 8) {
        return date + " " + time;
    }
    // 时间 "hh:mm:ss"：分隔位 ':'、其余全数字（编译期宏恒满足；防手传错形）
    bool timeOk = time[2] == ':' && time[5] == ':';
    for (size_t i = 0; i < time.size(); ++i) {
        if (i == 2 || i == 5) continue;
        if (time[i] < '0' || time[i] > '9') timeOk = false;
    }
    if (!timeOk) return date + " " + time;
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr",
                                          "May", "Jun", "Jul", "Aug",
                                          "Sep", "Oct", "Nov", "Dec"};
    int month = 0;
    for (int i = 0; i < 12; ++i) {
        if (date.compare(0, 3, kMonths[i]) == 0) {
            month = i + 1;
            break;
        }
    }
    if (month == 0) return date + " " + time;
    int day = 0;
    int year = 0;
    if (!parseAllDigits(date.substr(4, 2), day) ||
        !parseAllDigits(date.substr(7, 4), year) || day < 1 || day > 31 ||
        year < 1970 || year > 2999) {
        return date + " " + time;
    }
    auto pad2 = [](int v) {
        return (v < 10 ? std::string("0") : std::string()) + std::to_string(v);
    };
    return std::to_string(year) + "-" + pad2(month) + "-" + pad2(day) + " " + time;
}

std::string normalizeBuildSystemLabel(const std::string& raw) {
    const size_t space = raw.find(' ');
    const std::string name =
        space == std::string::npos ? raw : raw.substr(0, space);
    const std::string ver =
        space == std::string::npos ? std::string() : raw.substr(space + 1);
    if (name == "Windows") {
        // ver 形如 "10.0.26200"（major.minor.build）；build ≥22000 = Windows 11。
        const size_t dot1 = ver.find('.');
        const size_t dot2 =
            dot1 == std::string::npos ? std::string::npos : ver.find('.', dot1 + 1);
        if (dot1 != std::string::npos && dot2 != std::string::npos) {
            int build = 0;
            if (parseAllDigits(ver.substr(dot2 + 1), build)) {
                if (build >= 22000) return "Windows 11";
                if (build > 0) return "Windows 10";
            }
        }
        return ver.empty() ? "Windows" : "Windows " + ver;
    }
    if (name == "Darwin") {
        return ver.empty() ? "macOS" : "macOS " + ver;
    }
    return raw;
}

namespace {

// 版本信息行（本模块自给，不在 settings 内维护第二份）。
std::string aboutVersionLine() {
    return std::string("v") + DEVDESK_VERSION + " · " + DEVDESK_BUILD_TYPE;
}

// 构建行（UAT6 标准格式）：`年-月-日 时:分:秒@设备名@系统`
//（如 `2026-10-21 09:32:11@E-YUNFEI-PC@Windows 11`）。
std::string aboutBuildLine() {
    return formatBuildStamp(__DATE__, __TIME__) + "@" + DEVDESK_BUILD_HOST +
           "@" + normalizeBuildSystemLabel(DEVDESK_BUILD_SYSTEM);
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

// 信息行：左标签 + 右内容。highlight=true → 可复制（wechat.primary）。linkToast 非空=Ghost链接。
struct AboutRow {
    const char* label;
    std::string value;
    bool highlight = false;
    std::string linkToast;  // non-empty → render as ghost link, toast on click
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

        // S3.8 平铺版（2026-10-25 批J 用户拍板：彻底去卡片化，与其他页统一——
        // 页标题 + 分组内容直接铺在页面底上，无容器/描边/圆角/居中）
        constexpr float kGroupHdrH = 24.0f;    // 分组标题行高（16px/600 档 24px）
        constexpr float kGroupGap = 24.0f;     // 组间间距（1px 分隔线居中）
        constexpr float kRowH = 32.0f;         // 行高（S3.8#1）
        constexpr float kLabelW = 100.0f;      // 标签宽（S3.8#1）
        constexpr float kCopyW = 24.0f;        // 复制按钮宽
        constexpr float kFooterH = 20.0f;
        constexpr float kTitleH = 32.0f;       // 页标题行高（fontSizeTitle 档，同 agent.title）

        // 信息行结构：按分组组织
        struct InfoGroup {
            const char* header;
            std::vector<AboutRow> rows;
        };
        // S3.8#3: 三组——版本信息 / 构建信息 / 许可
        InfoGroup groups[] = {
            {"版本信息", {
                {"版本", aboutVersionLine(), true},
                {"Git commit", DEVDESK_GIT_REV, true},
                {"EUI-NEO", aboutEuiNeoLine(), true},
            }},
            {"构建信息", {
                {"构建时间", aboutBuildLine(), false},
                {"编译器", DEVDESK_COMPILER, false},
                {"平台", aboutPlatformLine(), false},
                {"C++ 标准", aboutCppLine(), false},
            }},
            {"许可与致谢", {
                {"开源协议", "MIT License", false},
                {"开源组件", "查看完整列表", false, "完整开源组件清单见仓库 LICENSE 说明"},
            }},
        };

        // S4.2 弹性：页内边距 32（spacingLarge×2）；内容固定高，最小窗 640 内不触 footer
        const float pagePad = t.sizes.spacingLarge * 2.0f;  // 32
        const float contentW = std::max(0.0f, screen.width - pagePad * 2.0f);
        const float footerY = screen.height - pagePad - kFooterH;

        // 页标题（与 agent.title 同款：fontSizeTitle 24/700、32 行高、左上）
        ui.text("about.title")
            .position(pagePad, 0.0f)  // 标题顶格不留空（2026-10-25 拍板）
            .size(contentW, kTitleH)
            .text("关于")
            .fontSize(t.sizes.fontSizeTitle)
            .lineHeight(kTitleH)
            .fontWeight(700)
            .color(t.colors.textPrimary)
            .build();

        // 平铺内容：分组标题/行/分隔线直接铺页面底，ID 从 about.card.* 迁 about.info.*
        const float valueX = kLabelW + 16.0f;
        float y = kTitleH + kGroupGap;
        for (size_t gi = 0; gi < std::size(groups); ++gi) {
            const auto& g = groups[gi];
            if (gi > 0) {
                // 组间 1px 分隔线（居中于 24 间距）
                ui.rect("about.info.divider." + std::to_string(gi))
                    .position(pagePad, y + kGroupGap * 0.5f - 0.5f)
                    .size(contentW, 1.0f)
                    .color(t.colors.borderDefault)
                    .build();
                y += kGroupGap;
            }
            // 分组标题：16px/600 textPrimary
            ui.text("about.info.group." + std::string(g.header))
                .position(pagePad, y)
                .size(contentW, kGroupHdrH)
                .text(g.header)
                .fontSize(t.sizes.fontSizeWeChat)  // 16px
                .lineHeight(kGroupHdrH)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            y += kGroupHdrH;

            for (size_t i = 0; i < g.rows.size(); ++i) {
                const float rowY = y + static_cast<float>(i) * kRowH;

                // 左标签：12px 三级字，右对齐，100px 宽
                // ID 必须含组维度 gi：框架按 ID 去重、后建覆盖先建，仅用行下标会跨组互覆丢行
                ui.text("about.info.label." + std::to_string(gi) + "." +
                        std::to_string(i))
                    .position(pagePad, rowY)
                    .size(kLabelW, kRowH)
                    .text(g.rows[i].label)
                    .fontSize(t.sizes.fontSizeSmall)  // 12px
                    .lineHeight(kRowH)
                    .color(t.colors.textTertiary)
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .build();

                // 右内容：14px/600，统一 textPrimary；highlight 仅控制复制图标显隐
                const float valueW = contentW - valueX - kCopyW;
                const bool isGhostLink = !g.rows[i].linkToast.empty();                ui.text("about.info.value." + std::to_string(gi) + "." +
                        std::to_string(i))
                    .position(pagePad + valueX, rowY)
                    .size(valueW, kRowH)
                    .text(g.rows[i].value)
                    .fontSize(t.sizes.fontSizeBody)  // 14px
                    .lineHeight(kRowH)
                    .fontWeight(600)
                    .color(isGhostLink ? t.colors.accentText
                                       : t.colors.textPrimary)
                    .build();

                // S3.8#2: Ghost 复制图标按钮（highlight 行）；热区收窄到图标区
                if (g.rows[i].highlight) {
                    const float iconX = pagePad + contentW - kCopyW;
                    const float iconY = rowY + (kRowH - 16.0f) * 0.5f;
                    ui.text("about.info.copy." + std::to_string(gi) + "." +
                            std::to_string(i))
                        .position(iconX, iconY)
                        .size(16.0f, 16.0f)
                        .icon(0xF0C5)  // fa-copy
                        .fontSize(core::theme::scaled(16.0f))
                        .color(t.colors.primary)
                        .build();

                    std::string copyText = g.rows[i].value;
                    ui.rect("about.info.hit." + std::to_string(gi) + "." +
                            std::to_string(i))
                        .position(pagePad + contentW - kCopyW, rowY)
                        .size(kCopyW, kRowH)
                        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                        .cursor(eui::CursorShape::Hand)
                        .onClick([this, copyText] {
                            ::core::window::setClipboardText(copyText);
                            triggerToast("已复制");
                            LOG_INFO("About: 已复制 " + copyText);
                        })
                        .build();
                }

                // Ghost 链接按钮（linkToast 非空行）
                if (!g.rows[i].linkToast.empty()) {
                    std::string toastMsg = g.rows[i].linkToast;
                    ui.rect("about.info.link." + std::to_string(gi) + "." +
                            std::to_string(i))
                        .position(pagePad + valueX, rowY)
                        .size(valueW, kRowH)
                        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                        .cursor(eui::CursorShape::Hand)
                        .onClick([this, toastMsg] {
                            triggerToast(toastMsg);
                            LOG_INFO("About: 开源组件链接点击");
                        })
                        .build();
                }
            }
            y += static_cast<float>(g.rows.size()) * kRowH;
        }

        // 版权声明
        ui.text("about.footer")
            .position(0.0f, footerY)
            .size(screen.width, kFooterH)
            .text("© 2026 DevDesk")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(kFooterH)
            .color(t.colors.textDisabled)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .build();

        // 复制成功 toast
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
