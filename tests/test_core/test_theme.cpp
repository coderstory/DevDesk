#include "doctest.h"
#include "core/component_theme.h"
#include "core/theme.h"

#include <components/dropdown.h>
#include <components/input.h>
#include <components/scroll.h>
#include <components/toast.h>
#include <eui/types.h>

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
