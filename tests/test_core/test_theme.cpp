#include "doctest.h"
#include "core/theme.h"

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