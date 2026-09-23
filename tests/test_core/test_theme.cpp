#include "doctest.h"
#include "core/component_theme.h"
#include "core/theme.h"

#include <components/dropdown.h>
#include <components/input.h>
#include <components/scroll.h>
#include <components/toast.h>
#include <eui/types.h>

#include <cmath>

REGISTER_TEST(theme_default_dark, "theme: default is dark", {
    DOCTEST_CHECK(core::theme::current().isDark);
    DOCTEST_CHECK_EQ(core::theme::currentThemeName(), std::string("Dark"));
    return true;
})

REGISTER_TEST(theme_set_light, "theme: setLight", {
    core::theme::setLight();
    DOCTEST_CHECK_FALSE(core::theme::current().isDark);
    DOCTEST_CHECK_EQ(core::theme::currentThemeName(), std::string("Light"));
    core::theme::setDark();
    DOCTEST_CHECK(core::theme::current().isDark);
    return true;
})

REGISTER_TEST(theme_tokens_values, "theme: tokens have values", {
    const auto& t = core::theme::current();
    DOCTEST_CHECK_EQ(t.sizes.fontSizeBody, 15.0f);
    DOCTEST_CHECK_EQ(t.sizes.fontSizeDisplay, 48.0f);
    DOCTEST_CHECK(t.sizes.radius >= 10.0f);
    DOCTEST_CHECK_EQ(t.sizes.spacing, 8.0f);
    return true;
})

REGISTER_TEST(theme_dark_light_differ, "theme: dark and light differ", {
    core::theme::setDark();
    auto darkBg = core::theme::current().colors.background;
    core::theme::setLight();
    auto lightBg = core::theme::current().colors.background;
    DOCTEST_CHECK(darkBg.r != lightBg.r || darkBg.g != lightBg.g || darkBg.b != lightBg.b);
    core::theme::setDark();
    return true;
})

// onPrimary token（W10 复审项）：主色背景上的前景色，两主题都必须
// 不透明且与主色有足够对比（主按钮文字可读），替代模块里 #FFFFFF 硬编码
REGISTER_TEST(theme_on_primary_readable, "theme: onPrimary opaque & contrasts primary", {
    core::theme::setDark();
    const auto& d = core::theme::current().colors;
    core::theme::setLight();
    const auto& l = core::theme::current().colors;
    const auto opaque = [](const eui::Color& c) { return c.a >= 0.999f; };
    // 与主色的亮度差（粗略相对亮度）：主按钮白字可读性下限
    const auto lumadiff = [](const eui::Color& a, const eui::Color& b) {
        const auto luma = [](const eui::Color& c) {
            return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
        };
        const float hi = luma(a) > luma(b) ? luma(a) : luma(b);
        const float lo = luma(a) > luma(b) ? luma(b) : luma(a);
        return hi - lo;
    };
    DOCTEST_CHECK(opaque(d.onPrimary));
    DOCTEST_CHECK(opaque(l.onPrimary));
    DOCTEST_CHECK(lumadiff(d.onPrimary, d.primary) > 0.4f);
    DOCTEST_CHECK(lumadiff(l.onPrimary, l.primary) > 0.4f);
    core::theme::setDark();
    return true;
})

// 标题栏深浅主题联动（磨砂设计 Phase A）：setDark/setLight 必须通知
// 回调（main.cpp 注入 app::setTitleBarAppearance），这里 mock 回调断言
// 通知发生且参数与 isDark 一致；清除后不再通知
REGISTER_TEST(theme_change_handler_notified, "theme: change handler notified on dark/light", {
    int calls = 0;
    bool sawDark = false;
    bool sawLight = false;
    bool lastNotifiedMatchesCurrent = false;
    core::theme::setChangeHandler([&](bool isDark) {
        ++calls;
        // 回调参数必须与切换后的主题一致（断言放外面：CHECK 宏带 return，
        // 不能进 void lambda）
        if (isDark) sawDark = true; else sawLight = true;
        lastNotifiedMatchesCurrent = (isDark == core::theme::current().isDark);
    });
    core::theme::setDark();
    DOCTEST_CHECK(lastNotifiedMatchesCurrent);
    core::theme::setLight();
    DOCTEST_CHECK(lastNotifiedMatchesCurrent);
    DOCTEST_CHECK_EQ(calls, 2);
    DOCTEST_CHECK(sawDark);
    DOCTEST_CHECK(sawLight);
    // 清除回调后切换不再通知
    core::theme::setChangeHandler(nullptr);
    core::theme::setDark();
    DOCTEST_CHECK_EQ(calls, 2);
    // 收尾：回到默认深色 + 无回调的全局状态
    core::theme::setDark();
    return true;
})
// ---- 色源统一（浅色米色/深色暖墨混杂 bug）：EUI-NEO 组件主题必须从
// core::theme 派生（componentTokens），不得回落内置 light()/dark() 默认。
// 断言策略：色值断言而非截图——两主题下关键组件（input/toast/dropdown）
// 的渲染色等于 core::theme 对应 token，且不等于 EUI-NEO 内置默认。
namespace {
bool sameRgb(const eui::Color& a, const eui::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}
} // namespace

REGISTER_TEST(theme_component_tokens_light_source, "theme: componentTokens derives EUI-NEO component colors from core tokens", {
    // 浅色：页面底米色 + 卡片暖白（spec §6.5.1 方案 B Light）
    core::theme::setLight();
    const auto& t = core::theme::current();
    const auto ct = core::theme::componentTokens(t);

    // 映射逐字段 == core token
    DOCTEST_CHECK(sameRgb(ct.background, t.colors.background));   // #FAF9F5
    DOCTEST_CHECK(sameRgb(ct.primary, t.colors.primary));         // #D97757
    DOCTEST_CHECK(sameRgb(ct.surface, t.colors.surface));         // #FFFFFF
    DOCTEST_CHECK(sameRgb(ct.surfaceHover, t.colors.hover));
    DOCTEST_CHECK(sameRgb(ct.surfaceActive, t.colors.active));
    DOCTEST_CHECK(sameRgb(ct.text, t.colors.textPrimary));
    DOCTEST_CHECK(sameRgb(ct.border, t.colors.borderDefault));
    DOCTEST_CHECK(ct.dark == t.isDark);

    // 关键组件实际渲染色 == core token（经组件 Style 构造的真实取色路径）
    const components::InputStyle input(ct);
    DOCTEST_CHECK(sameRgb(input.background, t.colors.surface));
    DOCTEST_CHECK(sameRgb(input.text, t.colors.textPrimary));
    DOCTEST_CHECK(sameRgb(input.focusBorder, t.colors.primary));
    const components::ToastStyle toast(ct);
    DOCTEST_CHECK(sameRgb(toast.background, t.colors.surface));   // 浅色 toast 面板 = 卡片白
    DOCTEST_CHECK(sameRgb(toast.accent, t.colors.primary));
    const components::DropdownStyle dd(ct);
    DOCTEST_CHECK(sameRgb(dd.field, t.colors.surface));
    DOCTEST_CHECK(sameRgb(dd.popup, t.colors.surface));           // 浅色弹层底 = 卡片白
    DOCTEST_CHECK(sameRgb(dd.text, t.colors.textPrimary));
    DOCTEST_CHECK(sameRgb(dd.accent, t.colors.primary));
    const components::ScrollStyle scroll(ct);

    // 不等于 EUI-NEO 内置默认（内置 light 是冷灰底 + 蓝主色，即本 bug 的杂色来源）
    const auto builtinLight = components::theme::light();
    DOCTEST_CHECK(!sameRgb(ct.background, builtinLight.background));
    DOCTEST_CHECK(!sameRgb(ct.primary, builtinLight.primary));
    DOCTEST_CHECK(!sameRgb(ct.text, builtinLight.text));
    (void)scroll;

    // 深色同断言（暖墨底 + Claude Clay，非内置冷灰黑/蓝）
    core::theme::setDark();
    const auto& td = core::theme::current();
    const auto ctd = core::theme::componentTokens(td);
    DOCTEST_CHECK(sameRgb(ctd.background, td.colors.background));  // #1A1816
    DOCTEST_CHECK(sameRgb(ctd.surface, td.colors.surface));        // #232120
    DOCTEST_CHECK(sameRgb(ctd.primary, td.colors.primary));        // #D97757（内置是蓝！）
    DOCTEST_CHECK(sameRgb(ctd.text, td.colors.textPrimary));
    const components::InputStyle inputD(ctd);
    DOCTEST_CHECK(sameRgb(inputD.background, td.colors.surface));
    const components::DropdownStyle ddD(ctd);
    DOCTEST_CHECK(sameRgb(ddD.field, td.colors.surface));
    DOCTEST_CHECK(sameRgb(ddD.accent, td.colors.primary));
    const auto builtinDark = components::theme::dark();
    DOCTEST_CHECK(!sameRgb(ctd.primary, builtinDark.primary));
    DOCTEST_CHECK(!sameRgb(ctd.surface, builtinDark.surface));

    core::theme::setLight();  // 交给下方用例前保持浅色无意义，恢复默认深色
    core::theme::setDark();
    return true;
})

// ---- 审计 [27]：浅色主题 WCAG AA 对比度护栏 ----
// 浅色 theme 是后加的、从未回验：旧 textTertiary #9A948A on #FAF9F5 只有
// 2.86:1（大量 11-13px 小字用），主色直接当文字 2.96/2.57:1。本测试把
// 「辅助灰 + primaryText 在所有落底上 ≥4.5:1」钉死为回归护栏（WCAG 2.x
// 相对亮度定义），并保住 secondary/tertiary/primary 三档亮度层级

namespace {

double wcagLuminance(const eui::Color& c) {
    const auto lin = [](double v) {
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
}

double wcagContrast(const eui::Color& a, const eui::Color& b) {
    const double la = wcagLuminance(a);
    const double lb = wcagLuminance(b);
    const double hi = la > lb ? la : lb;
    const double lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

} // namespace

REGISTER_TEST(theme_light_contrast_wcag, "theme: 浅色辅助灰/主色文字 WCAG AA（审计[27]）", {
    core::theme::setLight();
    const auto c = core::theme::current().colors;
    core::theme::setDark();
    const auto d = core::theme::current().colors;
    // 浅色小字三档：background / surface（卡片白）/ primarySoftBg（chip 底）
    DOCTEST_CHECK(wcagContrast(c.textTertiary, c.background) >= 4.5);
    DOCTEST_CHECK(wcagContrast(c.textTertiary, c.surface) >= 4.5);
    DOCTEST_CHECK(wcagContrast(c.textTertiary, c.primarySoftBg) >= 4.5);
    DOCTEST_CHECK(wcagContrast(c.textSecondary, c.background) >= 4.5);
    // 主色作文字（树选中/chip/大纲当前节/列表符号）走 primaryText
    DOCTEST_CHECK(wcagContrast(c.primaryText, c.background) >= 4.5);
    DOCTEST_CHECK(wcagContrast(c.primaryText, c.primarySoftBg) >= 4.5);
    // 辅助灰层级不能坍塌：浅底上文字越深越主要，亮度严格递增
    // textPrimary < textSecondary < textTertiary
    DOCTEST_CHECK(wcagLuminance(c.textPrimary) < wcagLuminance(c.textSecondary));
    DOCTEST_CHECK(wcagLuminance(c.textSecondary) < wcagLuminance(c.textTertiary));
    // 深色档数值未动，也进护栏防退化
    DOCTEST_CHECK(wcagContrast(d.textTertiary, d.background) >= 4.5);
    DOCTEST_CHECK(wcagContrast(d.primaryText, d.background) >= 4.5);
    DOCTEST_CHECK(wcagContrast(d.primaryText, d.primarySoftBg) >= 4.5);
    return true;
})
