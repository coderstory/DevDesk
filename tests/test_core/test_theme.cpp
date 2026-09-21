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