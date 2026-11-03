// 关于页独立模块（S3.8 UAT6.2 整改）。
// 标题左对齐 + 描述列表（标签/值层级）+ 信息分组，纯只读展示：无复制、无外链。
// 玻璃体系已退役（S6.1）。
// 平台/架构由 CMake 配置期注入宏，本文件只消费。
#include "core/component_theme.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/theme.h"

#include <eui/dsl.h>

#include <components/scrollview.h>
#include <ui_kit/card.h>

#include <algorithm>
#include <array>
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
    // 消费政策（ADR 0006）：不锁版本、每次用最新源码——只展示当前检出 rev，
    // 无「最低依赖 rev」；版本匹配由 scripts/check-eui-neo-version.sh 把关。
    return std::string("rev ") + DEVDESK_EUI_NEO_REV;
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

// 信息行：左标签 + 右内容（纯只读，无可交互元素）。
struct AboutRow {
    const char* label;
    std::string value;
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

        // F3 关于页卡片化（设计文档 2026-11-03 §14 / 池131）。
        // 拍板更替说明：本页 2026-10-25 批J 曾拍板「彻底去卡片化」（平铺无卡），
        // 2026-11-03 按新裁决翻回容器卡，本段几何以新拍板为准。
        // B6/F-69-2：行高/组距随字号派生——行盒不缩会让放大档「组标题压首行」
        //（实测 150% 档「构建信息」与「构建时间」叠字）。
        const float kGroupHdrH = core::theme::scaled(24.0f);  // 分组标题行高
        const float kGroupGap = core::theme::scaled(24.0f);   // 组间间距（1px 分隔线居中）
        const float kRowH = core::theme::scaled(32.0f);       // 行高（S3.8#1）
        const float kFooterH = core::theme::scaled(20.0f);
        const float kTitleH = core::theme::scaled(32.0f);     // 页标题行高
        constexpr float kLabelW = 100.0f;      // 标签宽（S3.8#1，列宽不随字号变）
        constexpr float kCardMaxW = 760.0f;    // 卡宽上限（min(760, screenW−64)）
        constexpr float kCardPad = 16.0f;      // 卡内边距（基线卡片内边距档）
        constexpr float kCardY = 24.0f;        // 卡顶距（页顶呼吸）

        // 信息行结构：按分组组织
        struct InfoGroup {
            const char* header;
            std::vector<AboutRow> rows;
        };
        // S3.8#3: 三组——版本信息 / 构建信息 / 许可
        InfoGroup groups[] = {
            {"版本信息", {
                {"版本", aboutVersionLine()},
                {"Git commit", DEVDESK_GIT_REV},
                {"EUI-NEO", aboutEuiNeoLine()},
            }},
            {"构建信息", {
                {"构建时间", aboutBuildLine()},
                {"编译器", DEVDESK_COMPILER},
                {"平台", aboutPlatformLine()},
                {"C++ 标准", aboutCppLine()},
            }},
            {"许可与致谢", {
                {"开源协议", "MIT License"},
            }},
        };

        // 卡几何（§14）：卡宽 min(760, screenW−64)；卡 x 同轴公式 → 卡心=页心=页脚心。
        // 卡高 = 内容高 + 上下内边距 16×2，钳制不撞页脚——最小窗 640×640：
        // footerY=588、卡底 488，余 100；放大档内容超出 → 钳高后卡内滚动。
        const float pagePad = t.sizes.spacingLarge * 2.0f;  // 32
        const float availW = std::max(0.0f, screen.width - pagePad * 2.0f);
        const float cardW = std::min(kCardMaxW, availW);
        const float cardX = pagePad + (availW - cardW) * 0.5f;
        const float footerY = screen.height - pagePad - kFooterH;

        // 内容高（行盒派生公式保留）：标题+间距+三组（组头+N×行，组间 24）。
        // 100% 档 = 32+24+(24+96)+24+(24+128)+24+(24+32) = 432 → 卡高 464。
        float contentH = kTitleH + kGroupGap;
        for (const auto& g : groups) {
            contentH += kGroupHdrH + static_cast<float>(g.rows.size()) * kRowH;
        }
        contentH += kGroupGap * static_cast<float>(std::size(groups) - 1);
        const float cardH = std::min(contentH + kCardPad * 2.0f,
                                     std::max(0.0f, footerY - kCardY - 24.0f));
        const float innerW = std::max(0.0f, cardW - kCardPad * 2.0f);
        const float contentTop = kCardY + kCardPad + kTitleH + kGroupGap;
        const float viewportH =
            std::max(0.0f, kCardY + cardH - kCardPad - contentTop);

        // 卡底板：cardSurface（池127 新值 0.94/0.95）+ radiusLarge + 1px
        // borderDefault + 全站统一轻阴影（ui_kit::cardPanel 单一来源）
        ui_kit::cardPanel(ui, "about.card", cardX, kCardY, cardW, cardH,
                          t.sizes.radiusLarge, t);

        // 页标题（卡内 y=卡Y+16 → 标题整体由 0 下移至 40；与 agent.title 同款：
        // fontSizeTitle 24/700、32 行高）
        ui.text("about.title")
            .position(cardX + kCardPad, kCardY + kCardPad)
            .size(innerW, kTitleH)
            .text("关于")
            .fontSize(t.sizes.fontSizeTitle)
            .lineHeight(kTitleH)
            .fontWeight(700)
            .color(t.colors.textPrimary)
            .build();

        // 组内容：卡内滚动区（100% 档恰好装满不出滚动条；放大档钳高内滚，
        // 滚动条 overlay 悬浮、不出卡边）。分组标题/行/分隔线坐标相对滚动区
        // 原点，ID 保持 about.info.*。
        const float valueX = kLabelW + 16.0f;
        components::scrollView(ui, "about.info.scroll")
            .position(cardX + kCardPad, contentTop)
            .size(innerW, viewportH)
            .gap(0.0f)
            .step(kRowH)
            .theme(core::theme::componentTokensWeChat(t))
            .scrollbarOverlay(true)
            .contentKey("about.info")
            .content([&](eui::Ui& inner, float w, float) {
                // 行=流式子件（stack/裸 text），行内子件绝对定位 + ignoreLayout——
                // 池133 习语：column 对未 ignoreLayout 子元素会静默忽略 position()，
                // 裸 position 文本会被流式布局竖着堆（label/value 叠行的根源）。
                for (size_t gi = 0; gi < std::size(groups); ++gi) {
                    const auto& g = groups[gi];
                    if (gi > 0) {
                        // 组间 1px 分隔线（居中于 24 间距）：gap 占位 stack + 段内 1px 线
                        inner.stack("about.info.gap." + std::to_string(gi))
                            .size(w, kGroupGap)
                            .content([&] {
                                inner.rect("about.info.divider." +
                                           std::to_string(gi))
                                    .position(0.0f, kGroupGap * 0.5f - 0.5f)
                                    .ignoreLayout()
                                    .size(w, 1.0f)
                                    .color(t.colors.borderDefault)
                                    .build();
                            })
                            .build();
                    }
                    // 分组标题：16px/600 textPrimary（流式子件，不写 position）
                    inner.text("about.info.group." + std::string(g.header))
                        .size(w, kGroupHdrH)
                        .text(g.header)
                        .fontSize(t.sizes.fontSizeWeChat)  // 16px
                        .lineHeight(kGroupHdrH)
                        .fontWeight(600)
                        .color(t.colors.textPrimary)
                        .build();

                    for (size_t i = 0; i < g.rows.size(); ++i) {
                        // 行 stack（流式占位）+ 行内 label/value 绝对定位。
                        // ID 必须含组维度 gi：框架按 ID 去重、后建覆盖先建，仅用行下标会跨组互覆丢行
                        const std::string sid =
                            std::to_string(gi) + "." + std::to_string(i);
                        inner.stack("about.info.row." + sid)
                            .size(w, kRowH)
                            .content([&] {
                                // 左标签：12px 三级字，右对齐，100px 宽
                                inner.text("about.info.label." + sid)
                                    .position(0.0f, 0.0f)
                                    .ignoreLayout()
                                    .size(kLabelW, kRowH)
                                    .text(g.rows[i].label)
                                    .fontSize(t.sizes.fontSizeSmall)  // 12px
                                    .lineHeight(kRowH)
                                    .color(t.colors.textTertiary)
                                    .horizontalAlign(eui::HorizontalAlign::Right)
                                    .build();

                                // 右内容：14px/600，统一 textPrimary（只读，无可交互元素）
                                // B6/F-69-2：值列必须闭包——放大档（实测 ≥130%）构建时间行超出
                                // 值列，EUI text 默认不裁会画到卡外被硬切。
                                inner.text("about.info.value." + sid)
                                    .position(valueX, 0.0f)
                                    .ignoreLayout()
                                    .size(w - valueX, kRowH)
                                    .text(g.rows[i].value)
                                    .fontSize(t.sizes.fontSizeBody)  // 14px
                                    .lineHeight(kRowH)
                                    .fontWeight(600)
                                    .maxWidth(w - valueX)
                                    .overflow(core::TextOverflow::Ellipsis)
                                    .clip()
                                    .color(t.colors.textPrimary)
                                    .build();
                            })
                            .build();
                    }
                }
            })
            .build();

        // 版权声明（页脚与卡天然同轴：卡心=页脚心=页面中线）
        ui.text("about.footer")
            .position(cardX, footerY)
            .size(cardW, kFooterH)
            .text("© 2026 DevDesk")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(kFooterH)
            .color(t.colors.textDisabled)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .build();
    }
};

} // namespace module::about

extern "C" ::core::Module* create_about_module() {
    return new module::about::AboutModule();
}
